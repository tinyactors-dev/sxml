/**
 * XSLT 4.0 value template grammar for sxml (docs/DECISIONS.md, D12, D29
 * and D97).
 *
 * Source of truth: XSLT 4.0, W3C QT4 CG draft, section 5.6 Value Templates
 * (https://qt4cg.org/specifications/xslt-40/Overview.html#value-templates).
 * One grammar serves attribute value templates (5.6.1) and text value
 * templates (5.6.2): an alternating sequence of fixed parts and variable
 * parts. Inherits the XPath 4.0 grammar: a variable part is "{" Expr? "}"
 * (the XPath enclosed_expression), so comments and string literals inside
 * it may hold unescaped curly brackets.
 *
 * The AST is the one of an XPath string template (D102): value_template
 * becomes the XPATH node, value_template_body its STRING_TEMPLATE body.
 */

const xpath = require('../xpath/grammar');

const valueTemplate = grammar(xpath, {
  name: 'value_template',

  // The shared token list of grammars/common/query_scanner.h, then the two
  // tokens of this grammar's scanner (grammars/value_template/src/scanner.c),
  // in this order.
  externals: ($, previous) => previous.concat([
    $.value_template_fixed_part,
    $._value_template_start,
  ]),

  rules: {
    // ValueTemplate ::= (FixedPart | VariablePart)*  (5.6)
    value_template: ($) => field('body', $.value_template_body),

    // _value_template_start is zero-width, so the body exists (and becomes
    // a STRING_TEMPLATE node) even for a zero-length template.
    value_template_body: ($) => seq(
      $._value_template_start,
      repeat(field('part', choice($.value_template_fixed_part, $.enclosed_expression))),
    ),
  },
});

// tree-sitter takes the first rule as the start rule; make it `value_template`.
valueTemplate.grammar.rules = Object.assign(
  { value_template: valueTemplate.grammar.rules.value_template },
  valueTemplate.grammar.rules,
);

module.exports = valueTemplate;
