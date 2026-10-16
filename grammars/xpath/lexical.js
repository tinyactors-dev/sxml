// Lexical building blocks shared by the XPath 4.0, XQuery 4.0 and XSLT 4.0
// pattern grammars (docs/DECISIONS.md, D29).
//
// XPath 4.0 A.3 Lexical structure: keywords are not reserved, so every keyword
// may also be used as a name. The grammars list the keywords here once and
// accept each of them wherever a name may appear (see `nameRules` in
// grammar.js).

// XML 1.0 (Fifth Edition) [4] NameStartChar, without ":" (XML Names NCName).
const NAME_START_CHARACTER =
  'A-Z_a-z\\u{C0}-\\u{D6}\\u{D8}-\\u{F6}\\u{F8}-\\u{2FF}\\u{370}-\\u{37D}' +
  '\\u{37F}-\\u{1FFF}\\u{200C}-\\u{200D}\\u{2070}-\\u{218F}\\u{2C00}-\\u{2FEF}' +
  '\\u{3001}-\\u{D7FF}\\u{F900}-\\u{FDCF}\\u{FDF0}-\\u{FFFD}\\u{10000}-\\u{EFFFF}';

// XML 1.0 [4a] NameChar, without ":".
const NAME_CHARACTER =
  NAME_START_CHARACTER + '\\-.0-9\\u{B7}\\u{300}-\\u{36F}\\u{203F}-\\u{2040}';

const NCNAME = `[${NAME_START_CHARACTER}][${NAME_CHARACTER}]*`;

// XPath 4.0 A.3.1: Digits ::= DecDigit ((DecDigit | "_")* DecDigit)?
const DIGITS = '[0-9](?:[0-9_]*[0-9])?';
const HEX_DIGITS = '[0-9a-fA-F](?:[0-9a-fA-F_]*[0-9a-fA-F])?';
const BINARY_DIGITS = '[01](?:[01_]*[01])?';

// XPath 4.0 A.3: the literal terminals spelled as names. Every one of them
// can also be an NCName.
const XPATH_KEYWORDS = [
  'ancestor', 'ancestor-or-self', 'and', 'array', 'as', 'at', 'attribute',
  'but', 'cast', 'castable', 'child', 'comment', 'declare', 'default',
  'descendant', 'descendant-or-self', 'div', 'document', 'document-node',
  'element', 'else', 'empty-sequence', 'enum', 'eq', 'every', 'except',
  'false', 'fn', 'following', 'following-or-self', 'following-sibling',
  'following-sibling-or-self', 'follows', 'follows-or-is', 'for', 'function',
  'ge', 'gt', 'idiv', 'if', 'in', 'instance', 'intersect', 'is', 'is-not',
  'item', 'jnode', 'key', 'le', 'let', 'lt', 'map', 'member', 'mod',
  'namespace', 'namespace-node', 'ne', 'node', 'of', 'or', 'otherwise',
  'parent', 'precedes', 'precedes-or-is', 'preceding', 'preceding-or-self',
  'preceding-sibling', 'preceding-sibling-or-self', 'processing-instruction',
  'record', 'return', 'satisfies', 'schema-attribute', 'schema-element',
  'self', 'some', 'text', 'then', 'to', 'treat', 'true', 'union', 'value',
  'with', 'xnode',
  // A.4 reserves these function names for compatibility with XQuery; they
  // are keywords here only so that the reserved-function-names rule can
  // exclude them.
  'switch', 'type', 'typeswitch',
];

// XQuery 4.0 A.3: further literal terminals spelled as names.
const XQUERY_KEYWORDS = [
  'allowing', 'ascending', 'base-uri', 'boundary-space', 'by', 'case',
  'catch', 'collation', 'construction', 'context', 'copy-namespaces',
  'count', 'decimal-format', 'decimal-separator', 'descending', 'digit',
  'empty', 'encoding', 'end', 'exponent-separator', 'external', 'finally',
  'fixed', 'greatest', 'group', 'grouping-separator', 'import', 'infinity',
  'inherit', 'lax', 'least', 'minus-sign', 'module', 'NaN', 'next',
  'no-inherit', 'no-preserve', 'only', 'option', 'order', 'ordered',
  'ordering', 'pattern-separator', 'per-mille', 'percent', 'preserve',
  'previous', 'schema', 'sliding', 'stable', 'start', 'strict', 'strip',
  'trace', 'try', 'tumbling', 'unordered', 'validate', 'variable', 'version',
  'when', 'where', 'while', 'window', 'xquery', 'zero-digit',
];

// XSLT 4.0 6.3 Patterns: further keywords.
const PATTERN_KEYWORDS = ['atomic'];

// XPath 4.0 A.4 Reserved Function Names: not allowed as unprefixed names of
// function calls and named function references.
const RESERVED_FUNCTION_NAMES = [
  'attribute', 'comment', 'document-node', 'element', 'namespace-node',
  'node', 'processing-instruction', 'schema-attribute', 'schema-element',
  'text', 'xnode', 'array', 'enum', 'fn', 'function', 'if', 'item', 'jnode',
  'map', 'record', 'switch', 'type', 'typeswitch',
];

// XQuery 4.0 A.1.2 Constraint unreserved-name: names that a computed
// constructor may not use unless written as a QName literal.
const CONSTRUCTOR_RESERVED_NAMES = [
  'and', 'but', 'case', 'div', 'else', 'eq', 'except', 'follows',
  'follows-or-is', 'for', 'ge', 'gt', 'idiv', 'intersect', 'is', 'is-not',
  'le', 'let', 'lt', 'mod', 'ne', 'or', 'otherwise', 'precedes',
  'precedes-or-is', 'return', 'satisfies', 'to', 'trace', 'union', 'where',
  'while',
];

module.exports = {
  NAME_START_CHARACTER,
  NAME_CHARACTER,
  NCNAME,
  DIGITS,
  HEX_DIGITS,
  BINARY_DIGITS,
  XPATH_KEYWORDS,
  XQUERY_KEYWORDS,
  PATTERN_KEYWORDS,
  RESERVED_FUNCTION_NAMES,
  CONSTRUCTOR_RESERVED_NAMES,
};
