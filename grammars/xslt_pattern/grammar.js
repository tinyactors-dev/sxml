/**
 * XSLT 4.0 pattern grammar for sxml (docs/DECISIONS.md, D12 and D29).
 *
 * Source of truth: XSLT 4.0, W3C QT4 CG draft, section 6.3 Patterns and
 * Appendix E Pattern Syntax Summary
 * (https://qt4cg.org/specifications/xslt-40/Overview.html#patterns).
 * Inherits the XPath 4.0 grammar: predicates hold full XPath expressions.
 * Rule names follow the pattern productions: a production ending in "P"
 * (a restricted XPath production) becomes `..._pattern`.
 */

const xpath = require('../xpath/grammar');
const lexical = require('../xpath/lexical');

const commaSeparated1 = (rule) => seq(rule, repeat(seq(',', rule)));
const commaSeparated = (rule) => optional(commaSeparated1(rule));
const separated1 = (separator, rule) => seq(rule, repeat(seq(separator, rule)));

const keywordAliases = ($, keywords) =>
  choice(...keywords.map((keyword) => alias(keyword, $.ncname)));

const without = (list, removed) => list.filter((entry) => !removed.includes(entry));

const KEYWORDS = lexical.XPATH_KEYWORDS.concat(lexical.PATTERN_KEYWORDS);

const xsltPattern = grammar(xpath, {
  name: 'xslt_pattern',

  reserved: {
    global: ($) => KEYWORDS,
  },

  rules: {
    // Pattern ::= XNodePattern | SimplePattern
    pattern: ($) => field('pattern', $._pattern),

    _pattern: ($) => choice($._union_pattern_level, $.simple_pattern),

    // UnionExprP ::= IntersectExceptExprP (("union" | "|") IntersectExceptExprP)*
    _union_pattern_level: ($) => choice($.union_pattern, $._intersect_except_pattern_level),
    union_pattern: ($) => seq(
      field('left', $._union_pattern_level),
      field('operator', choice('union', '|')),
      field('right', $._intersect_except_pattern_level),
    ),

    // IntersectExceptExprP ::= PathExprP (("intersect" | "except") PathExprP)*
    _intersect_except_pattern_level: ($) => choice($.intersect_except_pattern, $._path_pattern),
    intersect_except_pattern: ($) => seq(
      field('left', $._intersect_except_pattern_level),
      field('operator', choice('intersect', 'except')),
      field('right', $._path_pattern),
    ),

    // PathExprP ::= RootedPath | ("/" RelativePathExprP?) | ("//" RelativePathExprP) | RelativePathExprP
    _path_pattern: ($) => choice(
      $.rooted_path_pattern,
      $.absolute_path_pattern,
      $._relative_path_pattern_level,
    ),

    // RootedPath ::= VarRefXP PredicateXP* (("/" | "//") RelativePathExprP)?
    rooted_path_pattern: ($) => prec.right(seq(
      field('variable', $.variable_reference),
      repeat(field('predicate', $.predicate)),
      optional(seq(
        field('operator', choice('/', '//')),
        field('path', $._relative_path_pattern_level),
      )),
    )),

    // xgc: leading-lone-slash, as in XPath.
    absolute_path_pattern: ($) => choice(
      prec(-1, field('operator', '/')),
      seq(field('operator', '/'), field('path', $._relative_path_pattern_level)),
      seq(field('operator', '//'), field('path', $._relative_path_pattern_level)),
    ),

    // RelativePathExprP ::= StepExprP (("/" | "//") StepExprP)*
    _relative_path_pattern_level: ($) => choice($.relative_path_pattern, $._step_pattern),
    relative_path_pattern: ($) => seq(
      field('left', $._relative_path_pattern_level),
      field('operator', choice('/', '//')),
      field('right', $._step_pattern),
    ),

    // StepExprP ::= PostfixExprP | AxisStepP
    _step_pattern: ($) => choice($._postfix_pattern, $.axis_step_pattern),

    // PostfixExprP ::= (FunctionCallP | ParenthesizedPattern) PredicateXP*
    _postfix_pattern: ($) => choice(
      $.function_call_pattern,
      $.parenthesized_pattern,
      $.filter_pattern,
    ),

    filter_pattern: ($) => seq(
      field('value', choice($.function_call_pattern, $.parenthesized_pattern)),
      repeat1(field('predicate', $.predicate)),
    ),

    // FunctionCallP ::= OuterFunctionName ArgumentListP
    // ArgumentListP ::= "(" (ArgumentP ** ",") ")"; ArgumentP ::= VarRefXP | ConstantXP
    function_call_pattern: ($) => seq(
      field('name', $._function_eq_name),
      '(',
      commaSeparated(field('argument', choice($.variable_reference, $._constant))),
      ')',
    ),

    // ParenthesizedPattern ::= "(" Pattern ")"
    parenthesized_pattern: ($) => seq('(', field('pattern', $._pattern), ')'),

    // AxisStepP ::= ForwardStepP PredicateXP*
    // ForwardStepP ::= (ForwardAxisP NodeTestXP) | AbbrevForwardStepP
    // AbbrevForwardStepP ::= ("@" NodeTestXP) | SimpleNodeTestXP
    axis_step_pattern: ($) => seq(
      choice(
        seq(field('axis', $.forward_axis), field('test', $._node_test)),
        seq(field('abbreviation', '@'), field('test', $._node_test)),
        // A JNodeType is left out here: "jnode(" starts a JNodePattern.
        field('test', choice($.gnode_type, $._xnode_type, $._pattern_name_test, $.wildcard)),
      ),
      repeat(field('predicate', $.predicate)),
    ),

    _pattern_name_test: ($) => $._eq_name,

    // ForwardAxisP
    forward_axis: ($) => seq(
      field('name', choice('child', 'descendant', 'attribute', 'self', 'descendant-or-self', 'namespace')),
      '::',
    ),

    // SimplePattern ::= ItemPattern PredicateXP*
    simple_pattern: ($) => prec.right(seq(
      field('item', $._item_pattern),
      repeat(field('predicate', $.predicate)),
    )),

    // ItemPattern ::= AnyItemPattern | AtomicPattern | MapPattern | RecordPattern | ArrayPattern | JNodePattern
    _item_pattern: ($) => choice(
      alias($.context_value_reference, $.any_item_pattern),
      $.atomic_pattern,
      $.map_pattern,
      $.record_pattern,
      $.array_pattern,
      $.jnode_pattern,
    ),

    // AtomicPattern ::= "atomic" "(" (EQNameXP ++ "|") ")"
    atomic_pattern: ($) => seq('atomic', '(', separated1('|', field('type', $._eq_name)), ')'),

    // MapPattern ::= "map"? "{" (((MapEntryPattern ++ ",") ("," "*")?) | "*")? "}"
    map_pattern: ($) => seq(
      optional('map'), '{',
      optional(choice(
        seq(commaSeparated1(field('entry', $.map_entry_pattern)), optional(seq(',', field('open', '*')))),
        field('open', '*'),
      )),
      '}',
    ),

    // MapEntryPattern ::= MapEntryKey (":" SequencePattern)?; MapEntryKey ::= ConstantXP
    map_entry_pattern: ($) => seq(
      field('key', $._constant),
      optional(seq(':', field('value', $.sequence_pattern))),
    ),

    // RecordPattern ::= "record" "(" EQNameXP ")"
    record_pattern: ($) => seq('record', '(', field('name', $._eq_name), ')'),

    // ArrayPattern ::= "array" "(" SequencePattern ")"
    array_pattern: ($) => seq('array', '(', field('member', $.sequence_pattern), ')'),

    // JNodePattern ::= "jnode" "(" JNodePatternSelector "," SequencePattern ")"
    jnode_pattern: ($) => seq(
      'jnode', '(',
      field('selector', choice(alias('*', $.wildcard), $._constant)),
      ',',
      field('value', $.sequence_pattern),
      ')',
    ),

    // SequencePattern ::= ("empty-sequence" "(" ")") | (Pattern OccurrenceIndicatorXP?)
    sequence_pattern: ($) => choice(
      seq(field('empty', 'empty-sequence'), '(', ')'),
      prec.right(seq(field('pattern', $._pattern), optional(field('occurrence', $.occurrence_indicator)))),
    ),

    _keyword_name: ($) => keywordAliases($, KEYWORDS),

    // Only fn:doc, fn:id, fn:element-with-id, fn:key and fn:root may be called
    // at the outermost level, so "atomic" and "empty-sequence" (which start
    // patterns) are no function names here.
    _function_keyword_name: ($) => keywordAliases(
      $, without(KEYWORDS, lexical.RESERVED_FUNCTION_NAMES.concat(lexical.PATTERN_KEYWORDS, ['empty-sequence'])),
    ),
  },
});

// tree-sitter takes the first rule as the start rule; make it `pattern`.
xsltPattern.grammar.rules = Object.assign({ pattern: xsltPattern.grammar.rules.pattern }, xsltPattern.grammar.rules);

module.exports = xsltPattern;
